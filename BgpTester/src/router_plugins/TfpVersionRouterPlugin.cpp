#include "router_plugins/TfpVersionRouterPlugin.hpp"

#include "plugin/RouterPluginRegistry.hpp"
#include "router_plugins/StandardBgpRouterPlugin.hpp"

#include <QHash>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace bgptester
{
namespace
{

constexpr double MaxExactJsonInteger = 9007199254740991.0;

std::optional<quint64> versionFromJson(const QJsonValue& value)
{
    if (value.isUndefined() || value.isNull())
    {
        return 0;
    }
    if (value.isString())
    {
        bool ok = false;
        const auto result = value.toString().trimmed().toULongLong(&ok);
        return ok ? std::optional<quint64>(result) : std::nullopt;
    }
    if (value.isDouble())
    {
        const auto number = value.toDouble(-1.0);
        if (number >= 0.0 && number <= MaxExactJsonInteger && std::floor(number) == number)
        {
            return static_cast<quint64>(number);
        }
    }
    return std::nullopt;
}

QString configuredEntityId(const RouterNodeContext& context)
{
    const auto configured = context.config.pluginSettings.value(QStringLiteral("entity_id"));
    if (configured.isString() && !configured.toString().trimmed().isEmpty())
    {
        return configured.toString().trimmed();
    }
    return context.config.routerId;
}

bool classicEquivalent(const RouteEntry& lhs, const RouteEntry& rhs)
{
    const auto& left = lhs.attributes;
    const auto& right = rhs.attributes;
    return lhs.learnedFrom == rhs.learnedFrom && lhs.sourceSession == rhs.sourceSession &&
           lhs.localOrigin == rhs.localOrigin && lhs.source == rhs.source && left.origin == right.origin &&
           left.asPath == right.asPath && left.nextHop == right.nextHop &&
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
    for (auto it = triggers.cbegin(); it != triggers.cend(); ++it)
    {
        const auto dependency = dependencies.constFind(it.key());
        if (dependency != dependencies.cend() && dependency.value() < it.value())
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
        quint64 staleVersion = 0;
        quint64 seenVersion = 0;
        bool triggerSeen = false;
    };

    quint64 localVersion = 0;
    quint64 localSeenTriggerVersion = 0;
    bool advancedForPendingDecision = false;
    bool staleCheckNeeded = false;
    QHash<TfpEntity, quint64> maxVersions;
    QHash<TfpEntity, TriggerKnowledge> triggerKnowledge;
    TfpVersionVector pendingTriggers;
};

class TfpVersionRouterNode final : public StandardBgpRouterNode
{
public:
    explicit TfpVersionRouterNode(RouterNodeContext context, QObject* parent = nullptr)
        : StandardBgpRouterNode(std::move(context), parent), localEntity_{this->context().config.asn, configuredEntityId(this->context())},
          initialVersion_(versionFromJson(this->context().config.pluginSettings.value(QStringLiteral("initial_version"))).value_or(0))
    {
    }

    QStringList validateConfiguration() const override
    {
        auto problems = StandardBgpRouterNode::validateConfiguration();
        const auto settings = context().config.pluginSettings;
        const auto entityValue = settings.value(QStringLiteral("entity_id"));
        if (!entityValue.isUndefined() && !entityValue.isString())
        {
            problems.append(QStringLiteral("entity_id 必须是字符串"));
        }
        const auto parsedVersion = versionFromJson(settings.value(QStringLiteral("initial_version")));
        if (!parsedVersion || *parsedVersion == std::numeric_limits<quint64>::max())
        {
            problems.append(QStringLiteral("initial_version 必须是小于 18446744073709551615 的非负整数或十进制字符串"));
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

    RouteEntry createOriginatedRoute(const QString& prefix) override
    {
        auto route = StandardBgpRouterNode::createOriginatedRoute(prefix);
        auto& state = stateFor(prefix);
        if (bootstrapComplete_)
        {
            advanceLocalVersion(state);
            state.advancedForPendingDecision = true;
        }
        return route;
    }

    std::optional<RouteEntry> importRoute(const QString& prefix, const PathAttributes& attributes, const NeighborConfig& fromPeer) override
    {
        auto imported = StandardBgpRouterNode::importRoute(prefix, attributes, fromPeer);
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

    void importWithdrawal(const QString& prefix, const PathAttributes& attributes, const NeighborConfig& fromPeer) override
    {
        StandardBgpRouterNode::importWithdrawal(prefix, attributes, fromPeer);
        observeVersionInfo(prefix, attributes);
    }

    void localRouteWithdrawn(const QString& prefix) override
    {
        StandardBgpRouterNode::localRouteWithdrawn(prefix);
        auto& state = stateFor(prefix);
        if (bootstrapComplete_)
        {
            advanceLocalVersion(state);
            state.advancedForPendingDecision = true;
        }
    }

    std::optional<RouteEntry> selectBestRoute(const QString& prefix, const QVector<RouteEntry>& candidates,
                                              const std::optional<RouteEntry>& currentBest) override
    {
        auto& mutableState = stateFor(prefix);
        const auto& state = mutableState;
        decisionPrefix_ = prefix;
        decisionState_ = &mutableState;
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
            else if (samePrimaryPreference(candidate, *primaryBest) &&
                     deterministicRouteBetter(candidate, *deterministicBest))
            {
                deterministicBest = &candidate;
            }
            if (currentBest && !refreshedCurrent && classicEquivalent(candidate, *currentBest))
            {
                refreshedCurrent = &candidate;
            }
        }

        const RouteEntry* selectedRoute = nullptr;
        auto preservesCurrent = false;
        if (primaryBest)
        {
            // Preserve standard BGP's current-route stickiness while returning
            // the freshly imported dependency metadata for that same route.
            preservesCurrent = currentBest && refreshedCurrent && samePrimaryPreference(*refreshedCurrent, *primaryBest);
            selectedRoute = preservesCurrent ? refreshedCurrent : deterministicBest;
        }

        const auto classicRouteChanged = currentBest ? !preservesCurrent : selectedRoute != nullptr;
        if (bootstrapComplete_ && classicRouteChanged)
        {
            if (!mutableState.advancedForPendingDecision)
            {
                // If an inbound trigger explicitly proves the old selected
                // route stale, that trigger already invalidates every copy of
                // the old advertisement. Advance the durable local version,
                // but do not append a redundant hop-by-hop local cause.
                const auto publishLocalTrigger = !currentBest || !triggerInvalidates(*currentBest, mutableState.pendingTriggers);
                advanceLocalVersion(mutableState, publishLocalTrigger);
            }
        }
        return selectedRoute ? std::optional<RouteEntry>(*selectedRoute) : std::nullopt;
    }

    bool requiresDissemination(const QString& prefix) const override
    {
        if (decisionState_ && decisionPrefix_ == prefix)
        {
            return !decisionState_->pendingTriggers.isEmpty();
        }
        const auto state = states_.constFind(prefix);
        return state != states_.cend() && !state->pendingTriggers.isEmpty();
    }

    void decisionCompleted(const QString& prefix) override
    {
        PrefixVersionState* state = nullptr;
        if (decisionState_ && decisionPrefix_ == prefix)
        {
            state = decisionState_;
        }
        else if (const auto stored = states_.find(prefix); stored != states_.end())
        {
            state = &stored.value();
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

    std::optional<RouteEntry> exportRouteForPrefix(const QString& prefix, const RouteEntry& route, const NeighborConfig& toPeer) override
    {
        auto exported = StandardBgpRouterNode::exportRouteForPrefix(prefix, route, toPeer);
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
            info.dependencyVector.insert(localEntity_, state.localVersion);
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

    PathAttributes exportWithdrawal(const QString& prefix, const NeighborConfig& toPeer) override
    {
        auto attributes = StandardBgpRouterNode::exportWithdrawal(prefix, toPeer);
        auto& state = decisionState_ && decisionPrefix_ == prefix ? *decisionState_ : stateFor(prefix);
        if (!state.pendingTriggers.isEmpty())
        {
            TfpVersionInfo info;
            info.triggerVector = state.pendingTriggers;
            attributes.tfpVersionInfo = std::move(info);
        }
        return attributes;
    }

private:
    PrefixVersionState& stateFor(const QString& prefix)
    {
        if (cachedState_ && cachedPrefix_ == prefix)
        {
            return *cachedState_;
        }
        auto it = states_.find(prefix);
        if (it == states_.end())
        {
            PrefixVersionState state;
            // initial_version is the current persisted bootstrap baseline;
            // establishing it is not itself a version event.
            state.localVersion = initialVersion_;
            state.localSeenTriggerVersion = initialVersion_;
            state.staleCheckNeeded = initialVersion_ > 0;
            it = states_.insert(prefix, std::move(state));
        }
        cachedPrefix_ = prefix;
        cachedState_ = &it.value();
        return *cachedState_;
    }

    bool isStale(const RouteEntry& route, const PrefixVersionState& state) const
    {
        if (!route.attributes.tfpVersionInfo)
        {
            return false;
        }
        const auto& dependencies = route.attributes.tfpVersionInfo->dependencyVector;
        for (auto it = state.triggerKnowledge.cbegin(); it != state.triggerKnowledge.cend(); ++it)
        {
            if (it->staleVersion == 0)
            {
                continue;
            }
            const auto dependency = dependencies.constFind(it.key());
            if (dependency != dependencies.cend() && dependency.value() < it->staleVersion)
            {
                return true;
            }
        }
        if (state.localVersion != 0)
        {
            const auto localDependency = dependencies.constFind(localEntity_);
            if (localDependency != dependencies.cend() && localDependency.value() < state.localVersion)
            {
                return true;
            }
        }
        return false;
    }

    void observeVersionInfo(const QString& prefix, const PathAttributes& attributes)
    {
        if (!attributes.tfpVersionInfo)
        {
            return;
        }
        auto& state = decisionState_ && decisionPrefix_ == prefix ? *decisionState_ : stateFor(prefix);
        const auto mergeKnown = [&](const TfpVersionVector& versions)
        {
            for (auto it = versions.cbegin(); it != versions.cend(); ++it)
            {
                if (it.key().asn == 0 || it.key().entityId.isEmpty())
                {
                    continue;
                }
                if (it.key() == localEntity_)
                {
                    if (state.localVersion < it.value())
                    {
                        state.localVersion = it.value();
                        state.staleCheckNeeded = true;
                    }
                    continue;
                }
                const auto known = state.maxVersions.constFind(it.key());
                if (known == state.maxVersions.cend())
                {
                    // Zero is the minimum possible version and therefore adds
                    // no stale-route knowledge. The default bootstrap carries
                    // many such dependencies; materializing them in every
                    // prefix's QHash only inflates resident state.
                    if (it.value() == 0)
                    {
                        continue;
                    }
                    state.maxVersions.insert(it.key(), it.value());
                    // An omitted zero is the implicit lower baseline. The first
                    // positive Dependency must therefore activate the sparse
                    // frontier, just as the old materialized 0 -> value update
                    // did. This also advances a frontier created by a Trigger
                    // that arrived before the stable Dependency.
                    auto knowledge = state.triggerKnowledge.find(it.key());
                    if (knowledge == state.triggerKnowledge.end())
                    {
                        PrefixVersionState::TriggerKnowledge value;
                        value.staleVersion = it.value();
                        state.triggerKnowledge.insert(it.key(), value);
                    }
                    else if (knowledge->staleVersion < it.value())
                    {
                        knowledge->staleVersion = it.value();
                    }
                    state.staleCheckNeeded = true;
                }
                else if (known.value() != it.value())
                {
                    // A lower Dependency can arrive after a newer one through
                    // another peer.  The effective MaxVer is the maximum of
                    // both observations, so that late route must immediately
                    // activate the sparse stale frontier as well.
                    const auto effectiveVersion = std::max(known.value(), it.value());
                    if (known.value() < effectiveVersion)
                    {
                        state.maxVersions.insert(it.key(), effectiveVersion);
                    }
                    auto knowledge = state.triggerKnowledge.find(it.key());
                    if (knowledge == state.triggerKnowledge.end())
                    {
                        PrefixVersionState::TriggerKnowledge value;
                        value.staleVersion = effectiveVersion;
                        state.triggerKnowledge.insert(it.key(), value);
                    }
                    else if (knowledge->staleVersion < effectiveVersion)
                    {
                        knowledge->staleVersion = effectiveVersion;
                    }
                    state.staleCheckNeeded = true;
                }
            }
        };
        mergeKnown(attributes.tfpVersionInfo->dependencyVector);
        for (auto it = attributes.tfpVersionInfo->triggerVector.cbegin();
             it != attributes.tfpVersionInfo->triggerVector.cend(); ++it)
        {
            if (it.key().asn == 0 || it.key().entityId.isEmpty())
            {
                continue;
            }
            if (it.key() == localEntity_)
            {
                if (state.localSeenTriggerVersion >= it.value())
                {
                    continue;
                }
                state.localVersion = std::max(state.localVersion, it.value());
                state.localSeenTriggerVersion = it.value();
                state.staleCheckNeeded = true;
                state.pendingTriggers.insert(it.key(), it.value());
                continue;
            }
            auto& knowledge = state.triggerKnowledge[it.key()];
            if (knowledge.triggerSeen && knowledge.seenVersion >= it.value())
            {
                continue;
            }
            // Trigger knowledge is already durable in StaleFrontier and the
            // seen-trigger watermark. Dependency observations update MaxVer;
            // avoiding a third write here keeps the fault hot path sparse.
            state.staleCheckNeeded = true;
            const auto known = state.maxVersions.constFind(it.key());
            const auto effectiveVersion = known == state.maxVersions.cend() ? it.value() : std::max(known.value(), it.value());
            knowledge.staleVersion = std::max(knowledge.staleVersion, effectiveVersion);
            knowledge.seenVersion = it.value();
            knowledge.triggerSeen = true;
            if (state.pendingTriggers.isEmpty() && attributes.tfpVersionInfo->triggerVector.size() == 1)
            {
                // QMap is implicitly shared. The common single-cause case can
                // retain the validated wire delta without another tree lookup.
                state.pendingTriggers = attributes.tfpVersionInfo->triggerVector;
            }
            else
            {
                const auto pending = state.pendingTriggers.constFind(it.key());
                if (pending == state.pendingTriggers.cend() || pending.value() < it.value())
                {
                    state.pendingTriggers.insert(it.key(), it.value());
                }
            }
        }
    }

    void advanceLocalVersion(PrefixVersionState& state, bool publishTrigger = true)
    {
        if (state.localVersion < std::numeric_limits<quint64>::max())
        {
            ++state.localVersion;
            state.staleCheckNeeded = true;
            if (publishTrigger)
            {
                state.localSeenTriggerVersion = state.localVersion;
                if (state.pendingTriggers.isEmpty())
                {
                    auto canonical = localTriggerVectors_.find(state.localVersion);
                    if (canonical == localTriggerVectors_.end())
                    {
                        TfpVersionVector trigger;
                        trigger.insert(localEntity_, state.localVersion);
                        canonical = localTriggerVectors_.insert(state.localVersion, std::move(trigger));
                    }
                    state.pendingTriggers = canonical.value();
                }
                else
                {
                    state.pendingTriggers.insert(localEntity_, state.localVersion);
                }
            }
        }
    }

    TfpEntity localEntity_;
    quint64 initialVersion_ = 0;
    bool bootstrapComplete_ = false;
    QHash<QString, PrefixVersionState> states_;
    QHash<quint64, TfpVersionVector> localTriggerVectors_;
    QString decisionPrefix_;
    PrefixVersionState* decisionState_ = nullptr;
    std::optional<TfpVersionInfo> decisionExportVersionInfo_;
    QString cachedPrefix_;
    PrefixVersionState* cachedState_ = nullptr;
};

} // namespace

RouterPluginMetadata TfpVersionRouterPlugin::metadata() const
{
    return RouterPluginMetadata{
        .id = TfpVersionRouterPluginId,
        .displayName = QStringLiteral("TFP 路径版本路由器"),
        .version = QStringLiteral("1.2.0"),
        .description = QStringLiteral("基于路由器级实体、依赖向量和触发向量，提前排除必然失效的旧路径。"),
        .apiVersion = RouterPluginApiVersion,
        .defaultSettings = QJsonObject{{QStringLiteral("entity_id"), QString{}}, {QStringLiteral("initial_version"), QStringLiteral("0")}},
    };
}

RouterNode* TfpVersionRouterPlugin::createRouterNode(const RouterNodeContext& context, QObject* parent, QString* error)
{
    if (error)
    {
        error->clear();
    }
    return new TfpVersionRouterNode(context, parent);
}

} // namespace bgptester

BGPTESTER_REGISTER_ROUTER_PLUGIN(bgptester::TfpVersionRouterPlugin)
