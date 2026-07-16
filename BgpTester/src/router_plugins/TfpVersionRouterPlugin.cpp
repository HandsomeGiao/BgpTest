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

void mergeVersions(TfpVersionVector& destination, const TfpVersionVector& source)
{
    for (auto it = source.cbegin(); it != source.cend(); ++it)
    {
        if (it.key().asn == 0 || it.key().entityId.isEmpty())
        {
            continue;
        }
        auto current = destination.find(it.key());
        if (current == destination.end() || current.value() < it.value())
        {
            destination.insert(it.key(), it.value());
        }
    }
}

struct PrefixVersionState
{
    quint64 localVersion = 0;
    bool hasLocalVersion = false;
    bool advancedForPendingDecision = false;
    TfpVersionVector maxVersions;
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

    RouteEntry createOriginatedRoute(const QString& prefix) override
    {
        auto route = StandardBgpRouterNode::createOriginatedRoute(prefix);
        auto& state = stateFor(prefix);
        advanceLocalVersion(state);
        state.advancedForPendingDecision = true;
        return route;
    }

    std::optional<RouteEntry> importRoute(const QString& prefix, const PathAttributes& attributes, const NeighborConfig& fromPeer) override
    {
        auto imported = StandardBgpRouterNode::importRoute(prefix, attributes, fromPeer);
        if (imported)
        {
            observeVersionInfo(prefix, attributes);
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
        advanceLocalVersion(state);
        state.advancedForPendingDecision = true;
    }

    std::optional<RouteEntry> selectBestRoute(const QString& prefix, const QVector<RouteEntry>& candidates,
                                              const std::optional<RouteEntry>& currentBest) override
    {
        const auto& state = stateFor(prefix);
        QVector<RouteEntry> eligible;
        eligible.reserve(candidates.size());
        for (const auto& candidate : candidates)
        {
            if (!isStale(candidate, state.maxVersions))
            {
                eligible.append(candidate);
            }
        }

        auto selected = StandardBgpRouterNode::selectBestRoute(prefix, eligible, currentBest);
        auto& mutableState = stateFor(prefix);
        if (selected != currentBest)
        {
            if (!mutableState.advancedForPendingDecision)
            {
                advanceLocalVersion(mutableState);
            }
        }
        mutableState.advancedForPendingDecision = false;
        return selected;
    }

    std::optional<RouteEntry> exportRouteForPrefix(const QString& prefix, const RouteEntry& route, const NeighborConfig& toPeer) override
    {
        auto exported = StandardBgpRouterNode::exportRouteForPrefix(prefix, route, toPeer);
        if (!exported)
        {
            return std::nullopt;
        }

        auto& state = stateFor(prefix);
        ensureLocalVersion(state);
        auto info = route.attributes.tfpVersionInfo.value_or(TfpVersionInfo{});
        info.dependencyVector.insert(localEntity_, state.localVersion);
        mergeVersions(info.triggerVector, state.maxVersions);
        info.triggerVector.insert(localEntity_, state.localVersion);
        exported->attributes.tfpVersionInfo = std::move(info);
        return exported;
    }

    PathAttributes exportWithdrawal(const QString& prefix, const NeighborConfig& toPeer) override
    {
        auto attributes = StandardBgpRouterNode::exportWithdrawal(prefix, toPeer);
        auto& state = stateFor(prefix);
        ensureLocalVersion(state);
        auto info = attributes.tfpVersionInfo.value_or(TfpVersionInfo{});
        mergeVersions(info.triggerVector, state.maxVersions);
        info.triggerVector.insert(localEntity_, state.localVersion);
        attributes.tfpVersionInfo = std::move(info);
        return attributes;
    }

private:
    PrefixVersionState& stateFor(const QString& prefix)
    {
        auto it = states_.find(prefix);
        if (it == states_.end())
        {
            PrefixVersionState state;
            state.localVersion = initialVersion_;
            it = states_.insert(prefix, std::move(state));
        }
        return it.value();
    }

    static bool isStale(const RouteEntry& route, const TfpVersionVector& maxVersions)
    {
        if (!route.attributes.tfpVersionInfo)
        {
            return false;
        }
        for (auto it = route.attributes.tfpVersionInfo->dependencyVector.cbegin();
             it != route.attributes.tfpVersionInfo->dependencyVector.cend(); ++it)
        {
            const auto known = maxVersions.constFind(it.key());
            if (known != maxVersions.cend() && it.value() < known.value())
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
        auto& state = stateFor(prefix);
        mergeVersions(state.maxVersions, attributes.tfpVersionInfo->dependencyVector);
        mergeVersions(state.maxVersions, attributes.tfpVersionInfo->triggerVector);
    }

    void advanceLocalVersion(PrefixVersionState& state)
    {
        if (!state.hasLocalVersion)
        {
            state.hasLocalVersion = true;
        }
        if (state.localVersion < std::numeric_limits<quint64>::max())
        {
            ++state.localVersion;
        }
        state.maxVersions.insert(localEntity_, state.localVersion);
    }

    void ensureLocalVersion(PrefixVersionState& state)
    {
        if (!state.hasLocalVersion)
        {
            advanceLocalVersion(state);
        }
    }

    TfpEntity localEntity_;
    quint64 initialVersion_ = 0;
    QHash<QString, PrefixVersionState> states_;
};

} // namespace

RouterPluginMetadata TfpVersionRouterPlugin::metadata() const
{
    return RouterPluginMetadata{
        .id = TfpVersionRouterPluginId,
        .displayName = QStringLiteral("TFP 路径版本路由器"),
        .version = QStringLiteral("1.0.0"),
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
