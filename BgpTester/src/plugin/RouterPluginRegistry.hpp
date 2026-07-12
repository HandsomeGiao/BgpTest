#pragma once

#include "plugin/RouterPlugin.hpp"

#include <QMap>
#include <QReadWriteLock>
#include <QStringList>
#include <QVector>

#include <optional>

namespace bgptester
{

struct RegisteredRouterPlugin
{
    RouterPluginMetadata metadata;
    QString source;
};

class RouterPluginRegistry final
{
public:
    static RouterPluginRegistry& instance();

    RouterPluginRegistry(const RouterPluginRegistry&) = delete;
    RouterPluginRegistry& operator=(const RouterPluginRegistry&) = delete;
    ~RouterPluginRegistry();

    [[nodiscard]] QVector<RegisteredRouterPlugin> plugins() const;
    [[nodiscard]] std::optional<RouterPluginMetadata> metadata(const QString& pluginId) const;
    [[nodiscard]] bool contains(const QString& pluginId) const;
    [[nodiscard]] QStringList registrationErrors() const;

    // Called by BGPTESTER_REGISTER_ROUTER_PLUGIN during process startup.
    // Plugin objects have static storage duration and remain owned by their
    // translation units.
    bool registerStaticPlugin(RouterNodePlugin* plugin, const QString& source);

    [[nodiscard]] RouterNode* createRouterNode(const RouterConfig& config, const Topology& topology, QObject* parent,
                                               QString* error = nullptr) const;

private:
    RouterPluginRegistry();

    struct Entry
    {
        RouterPluginMetadata metadata;
        RouterNodePlugin* factory = nullptr;
        QString source;
    };

    bool registerPlugin(RouterNodePlugin* plugin, const QString& source, QString* error);

    mutable QReadWriteLock lock_;
    QMap<QString, Entry> entries_;
    QStringList registrationErrors_;
};

} // namespace bgptester

#define BGPTESTER_DETAIL_JOIN_IMPL(lhs, rhs) lhs##rhs
#define BGPTESTER_DETAIL_JOIN(lhs, rhs) BGPTESTER_DETAIL_JOIN_IMPL(lhs, rhs)

// Put this once at the end of a router plugin .cpp file. CMake automatically
// compiles every .cpp under src/router_plugins, so no build-file edit or
// runtime loading step is required.
#define BGPTESTER_REGISTER_ROUTER_PLUGIN(PluginType)                                                                                       \
    namespace                                                                                                                              \
    {                                                                                                                                      \
    [[maybe_unused]] const bool BGPTESTER_DETAIL_JOIN(bgptesterRouterPluginRegistered_, __LINE__) = []                                     \
    {                                                                                                                                      \
        static PluginType plugin;                                                                                                          \
        return ::bgptester::RouterPluginRegistry::instance().registerStaticPlugin(&plugin, QString::fromUtf8(__FILE__));                   \
    }();                                                                                                                                   \
    }
