#include "plugin/RouterPluginRegistry.hpp"

#include <QDebug>

#include <algorithm>
#include <exception>
#include <utility>

namespace bgptester
{

RouterPluginRegistry& RouterPluginRegistry::instance()
{
    static RouterPluginRegistry registry;
    return registry;
}

RouterPluginRegistry::RouterPluginRegistry() = default;

RouterPluginRegistry::~RouterPluginRegistry() = default;

QVector<RegisteredRouterPlugin> RouterPluginRegistry::plugins() const
{
    QReadLocker locker(&lock_);
    QVector<RegisteredRouterPlugin> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_)
    {
        result.append(RegisteredRouterPlugin{.metadata = entry.metadata, .source = entry.source});
    }
    std::sort(result.begin(), result.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.metadata.displayName.localeAwareCompare(rhs.metadata.displayName) < 0; });
    return result;
}

std::optional<RouterPluginMetadata> RouterPluginRegistry::metadata(const QString& pluginId) const
{
    QReadLocker locker(&lock_);
    const auto it = entries_.constFind(pluginId);
    if (it == entries_.cend())
    {
        return std::nullopt;
    }
    return it->metadata;
}

bool RouterPluginRegistry::contains(const QString& pluginId) const
{
    QReadLocker locker(&lock_);
    return entries_.contains(pluginId);
}

QStringList RouterPluginRegistry::registrationErrors() const
{
    QReadLocker locker(&lock_);
    return registrationErrors_;
}

bool RouterPluginRegistry::registerStaticPlugin(RouterNodePlugin* plugin, const QString& source)
{
    QString error;
    if (registerPlugin(plugin, source, &error))
    {
        return true;
    }
    {
        QWriteLocker locker(&lock_);
        registrationErrors_.append(error);
    }
    qWarning().noquote() << error;
    return false;
}

RouterNode* RouterPluginRegistry::createRouterNode(const RouterConfig& config, const Topology& topology, QObject* parent,
                                                   QString* error) const
{
    QMap<QString, NeighborConfig> neighbors;
    for (const auto& neighbor : topology.neighborsFor(config.id))
    {
        neighbors.insert(neighbor.id, neighbor);
    }
    const RouterNodeContext context{
        .config = config,
        .topologyRouters = topology.routers,
        .neighbors = std::move(neighbors),
    };
    return createRouterNode(context, parent, error);
}

RouterNode* RouterPluginRegistry::createRouterNode(const RouterNodeContext& context, QObject* parent, QString* error) const
{
    if (error)
    {
        error->clear();
    }
    RouterNodePlugin* factory = nullptr;
    {
        QReadLocker locker(&lock_);
        const auto it = entries_.constFind(context.config.pluginId);
        if (it == entries_.cend())
        {
            if (error)
            {
                *error = QStringLiteral("路由器 %1 使用的插件未注册：%2").arg(context.config.id, context.config.pluginId);
            }
            return nullptr;
        }
        factory = it->factory;
    }

    try
    {
        auto* node = factory->createRouterNode(context, parent, error);
        if (!node && error && error->isEmpty())
        {
            *error = QStringLiteral("插件 %1 未能创建路由器 %2").arg(context.config.pluginId, context.config.id);
        }
        return node;
    }
    catch (const std::exception& exception)
    {
        if (error)
        {
            *error =
                QStringLiteral("插件 %1 创建路由器 %2 时发生异常：%3")
                    .arg(context.config.pluginId, context.config.id, QString::fromUtf8(exception.what()));
        }
    }
    catch (...)
    {
        if (error)
        {
            *error = QStringLiteral("插件 %1 创建路由器 %2 时发生未知异常").arg(context.config.pluginId, context.config.id);
        }
    }
    return nullptr;
}

bool RouterPluginRegistry::registerPlugin(RouterNodePlugin* plugin, const QString& source, QString* error)
{
    if (!plugin)
    {
        if (error)
        {
            *error = QStringLiteral("路由器插件工厂为空");
        }
        return false;
    }
    RouterPluginMetadata metadata;
    try
    {
        metadata = plugin->metadata();
    }
    catch (const std::exception& exception)
    {
        if (error)
        {
            *error = QStringLiteral("读取 %1 的插件元数据时发生异常：%2").arg(source, QString::fromUtf8(exception.what()));
        }
        return false;
    }
    catch (...)
    {
        if (error)
        {
            *error = QStringLiteral("读取 %1 的插件元数据时发生未知异常").arg(source);
        }
        return false;
    }
    const auto id = metadata.id.trimmed();
    if (id.isEmpty() || metadata.displayName.trimmed().isEmpty())
    {
        if (error)
        {
            *error = QStringLiteral("%1 的插件 ID 或显示名称为空").arg(source);
        }
        return false;
    }
    if (metadata.apiVersion != RouterPluginApiVersion)
    {
        if (error)
        {
            *error = QStringLiteral("插件 %1 的 API 版本为 %2，程序要求 %3").arg(id).arg(metadata.apiVersion).arg(RouterPluginApiVersion);
        }
        return false;
    }

    QWriteLocker locker(&lock_);
    if (entries_.contains(id))
    {
        if (error)
        {
            *error = QStringLiteral("路由器插件 ID 重复：%1（%2）").arg(id, source);
        }
        return false;
    }
    auto normalized = metadata;
    normalized.id = id;
    entries_.insert(id, Entry{.metadata = std::move(normalized), .factory = plugin, .source = source});
    return true;
}

} // namespace bgptester
