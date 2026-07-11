#include "plugin/RouterPlugin.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

namespace {

using namespace bgptester;

class ConfigurableExportRouterNode final : public RouterNode {
public:
  explicit ConfigurableExportRouterNode(RouterNodeContext context,
                                        QObject *parent = nullptr)
      : RouterNode(std::move(context), parent) {}

  QStringList validateConfiguration() const override {
    QStringList problems;
    const auto settings = context().config.pluginSettings;
    if (settings.contains(QStringLiteral("export_routes")) &&
        !settings.value(QStringLiteral("export_routes")).isBool()) {
      problems.append(QStringLiteral("export_routes 必须是布尔值"));
    }
    if (settings.contains(QStringLiteral("local_preference"))) {
      const auto value = settings.value(QStringLiteral("local_preference"));
      const auto number = value.toDouble(-1.0);
      if (!value.isDouble() || number < 0.0 ||
          number > std::numeric_limits<quint32>::max() ||
          std::floor(number) != number) {
        problems.append(
            QStringLiteral("local_preference 必须是 0..4294967295 的整数"));
      }
    }
    return problems;
  }

  RouteEntry createOriginatedRoute(const QString &prefix) override {
    RouteEntry route;
    route.prefix = prefix;
    route.attributes.nextHop = context().config.routerId;
    route.attributes.localPref = static_cast<quint32>(
        context().config.pluginSettings
            .value(QStringLiteral("local_preference"))
            .toInteger(100));
    route.learnedFrom = context().config.id;
    route.localOrigin = true;
    return route;
  }

  std::optional<RouteEntry>
  importRoute(const QString &prefix, const PathAttributes &attributes,
              const NeighborConfig &fromPeer) override {
    if (attributes.asPath.contains(context().config.asn)) {
      return std::nullopt;
    }
    RouteEntry route;
    route.prefix = prefix;
    route.attributes = attributes;
    route.learnedFrom = fromPeer.id;
    route.sourceSession = fromPeer.sessionType;
    return route;
  }

  std::optional<RouteEntry>
  selectBestRoute(const QString &, const QVector<RouteEntry> &candidates,
                  const std::optional<RouteEntry> &) override {
    if (candidates.isEmpty()) {
      return std::nullopt;
    }
    return *std::min_element(
        candidates.cbegin(), candidates.cend(),
        [](const RouteEntry &lhs, const RouteEntry &rhs) {
          // Deliberately simple custom policy: local, then LOCAL_PREF, then
          // shortest AS_PATH, followed by a stable peer-id tie break.
          return std::tuple(!lhs.localOrigin,
                            std::numeric_limits<quint32>::max() -
                                lhs.attributes.localPref,
                            lhs.attributes.asPath.size(), lhs.learnedFrom) <
                 std::tuple(!rhs.localOrigin,
                            std::numeric_limits<quint32>::max() -
                                rhs.attributes.localPref,
                            rhs.attributes.asPath.size(), rhs.learnedFrom);
        });
  }

  std::optional<RouteEntry>
  exportRoute(const RouteEntry &route,
              const NeighborConfig &toPeer) override {
    if (!context().config.pluginSettings
             .value(QStringLiteral("export_routes"))
             .toBool(false) ||
        route.learnedFrom == toPeer.id) {
      return std::nullopt;
    }
    if (toPeer.sessionType == SessionType::Ibgp && !route.localOrigin &&
        route.sourceSession == SessionType::Ibgp) {
      return std::nullopt;
    }

    auto exported = route;
    exported.learnedFrom = context().config.id;
    exported.localOrigin = false;
    exported.sourceSession = toPeer.sessionType;
    if (toPeer.sessionType == SessionType::Ebgp) {
      exported.attributes.asPath.prepend(context().config.asn);
      exported.attributes.nextHop = context().config.routerId;
    }
    return exported;
  }
};

class ConfigurableExportRouterPlugin final : public QObject,
                                             public RouterNodePlugin {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID BGPTESTER_ROUTER_PLUGIN_IID)
  Q_INTERFACES(bgptester::RouterNodePlugin)

public:
  RouterPluginMetadata metadata() const override {
    return RouterPluginMetadata{
        .id = QStringLiteral("org.bgptester.example.configurable-export"),
        .displayName = QStringLiteral("示例：可配置出口路由器"),
        .version = QStringLiteral("1.0.0"),
        .description = QStringLiteral(
            "演示自定义选路与出口控制；export_routes 决定是否发布路由。"),
        .apiVersion = RouterPluginApiVersion,
        .defaultSettings =
            QJsonObject{{QStringLiteral("export_routes"), false},
                        {QStringLiteral("local_preference"), 100}},
    };
  }

  RouterNode *createRouterNode(const RouterNodeContext &context,
                               QObject *parent, QString *error) override {
    if (error) {
      error->clear();
    }
    return new ConfigurableExportRouterNode(context, parent);
  }
};

} // namespace

#include "ConfigurableExportRouterPlugin.moc"
