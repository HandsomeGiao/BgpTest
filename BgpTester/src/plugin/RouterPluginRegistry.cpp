#include "plugin/RouterPluginRegistry.hpp"

#include "plugin/StandardBgpRouterPlugin.hpp"

#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QPluginLoader>

#include <algorithm>
#include <exception>
#include <utility>

namespace bgptester {

RouterPluginRegistry &RouterPluginRegistry::instance() {
  static RouterPluginRegistry registry;
  return registry;
}

RouterPluginRegistry::RouterPluginRegistry()
    : standardPlugin_(std::make_unique<StandardBgpRouterPlugin>()) {
  QString error;
  registerPlugin(standardPlugin_.get(), QStringLiteral("built-in"), &error);
}

RouterPluginRegistry::~RouterPluginRegistry() {
  {
    QWriteLocker locker(&lock_);
    entries_.clear();
  }
  for (auto *loader : std::as_const(loaders_)) {
    loader->unload();
    delete loader;
  }
}

QVector<RegisteredRouterPlugin> RouterPluginRegistry::plugins() const {
  QReadLocker locker(&lock_);
  QVector<RegisteredRouterPlugin> result;
  result.reserve(entries_.size());
  for (const auto &entry : entries_) {
    result.append(
        RegisteredRouterPlugin{.metadata = entry.metadata,
                               .source = entry.source});
  }
  std::sort(result.begin(), result.end(), [](const auto &lhs, const auto &rhs) {
    return lhs.metadata.displayName.localeAwareCompare(
               rhs.metadata.displayName) < 0;
  });
  return result;
}

std::optional<RouterPluginMetadata>
RouterPluginRegistry::metadata(const QString &pluginId) const {
  QReadLocker locker(&lock_);
  const auto it = entries_.constFind(pluginId);
  if (it == entries_.cend()) {
    return std::nullopt;
  }
  return it->metadata;
}

bool RouterPluginRegistry::contains(const QString &pluginId) const {
  QReadLocker locker(&lock_);
  return entries_.contains(pluginId);
}

bool RouterPluginRegistry::loadPlugin(const QString &filePath,
                                      QString *error) {
  if (error) {
    error->clear();
  }
  const auto info = QFileInfo(filePath);
  const auto canonical = info.canonicalFilePath();
  if (!info.exists() || !info.isFile() || canonical.isEmpty()) {
    if (error) {
      *error = QStringLiteral("插件文件不存在：%1").arg(filePath);
    }
    return false;
  }
  {
    QReadLocker locker(&lock_);
    if (loadedFiles_.contains(canonical)) {
      return true;
    }
  }

  auto *loader = new QPluginLoader(canonical);
  auto *instance = loader->instance();
  if (!instance) {
    if (error) {
      *error = QStringLiteral("无法加载路由器插件 %1：%2")
                   .arg(canonical, loader->errorString());
    }
    delete loader;
    return false;
  }
  auto *plugin = qobject_cast<RouterNodePlugin *>(instance);
  if (!plugin) {
    if (error) {
      *error = QStringLiteral("%1 不是 BgpTester 路由器插件（IID %2）")
                   .arg(canonical, QStringLiteral(BGPTESTER_ROUTER_PLUGIN_IID));
    }
    loader->unload();
    delete loader;
    return false;
  }

  QString registrationError;
  if (!registerPlugin(plugin, canonical, &registrationError)) {
    if (error) {
      *error = registrationError;
    }
    loader->unload();
    delete loader;
    return false;
  }
  {
    QWriteLocker locker(&lock_);
    loadedFiles_.insert(canonical);
    loaders_.append(loader);
  }
  return true;
}

QStringList
RouterPluginRegistry::loadDirectory(const QString &directoryPath) {
  QStringList errors;
  QDir directory(directoryPath);
  if (!directory.exists()) {
    return errors;
  }
  const auto files = directory.entryInfoList(QDir::Files | QDir::Readable,
                                             QDir::Name);
  for (const auto &file : files) {
    if (!QLibrary::isLibrary(file.absoluteFilePath())) {
      continue;
    }
    QString error;
    if (!loadPlugin(file.absoluteFilePath(), &error)) {
      errors.append(error);
    }
  }
  return errors;
}

RouterNode *RouterPluginRegistry::createRouterNode(
    const RouterConfig &config, const Topology &topology, QObject *parent,
    QString *error) const {
  if (error) {
    error->clear();
  }
  RouterNodePlugin *factory = nullptr;
  {
    QReadLocker locker(&lock_);
    const auto it = entries_.constFind(config.pluginId);
    if (it == entries_.cend()) {
      if (error) {
        *error = QStringLiteral("路由器 %1 使用的插件未加载：%2")
                     .arg(config.id, config.pluginId);
      }
      return nullptr;
    }
    factory = it->factory;
  }

  QMap<QString, NeighborConfig> neighbors;
  for (const auto &neighbor : topology.neighborsFor(config.id)) {
    neighbors.insert(neighbor.id, neighbor);
  }
  const RouterNodeContext context{
      .config = config,
      .topologyRouters = topology.routers,
      .neighbors = std::move(neighbors),
  };

  try {
    auto *node = factory->createRouterNode(context, parent, error);
    if (!node && error && error->isEmpty()) {
      *error = QStringLiteral("插件 %1 未能创建路由器 %2")
                   .arg(config.pluginId, config.id);
    }
    return node;
  } catch (const std::exception &exception) {
    if (error) {
      *error = QStringLiteral("插件 %1 创建路由器 %2 时发生异常：%3")
                   .arg(config.pluginId, config.id,
                        QString::fromUtf8(exception.what()));
    }
  } catch (...) {
    if (error) {
      *error = QStringLiteral("插件 %1 创建路由器 %2 时发生未知异常")
                   .arg(config.pluginId, config.id);
    }
  }
  return nullptr;
}

bool RouterPluginRegistry::registerPlugin(RouterNodePlugin *plugin,
                                          const QString &source,
                                          QString *error) {
  if (!plugin) {
    if (error) {
      *error = QStringLiteral("路由器插件工厂为空");
    }
    return false;
  }
  RouterPluginMetadata metadata;
  try {
    metadata = plugin->metadata();
  } catch (const std::exception &exception) {
    if (error) {
      *error = QStringLiteral("读取 %1 的插件元数据时发生异常：%2")
                   .arg(source, QString::fromUtf8(exception.what()));
    }
    return false;
  } catch (...) {
    if (error) {
      *error = QStringLiteral("读取 %1 的插件元数据时发生未知异常")
                   .arg(source);
    }
    return false;
  }
  const auto id = metadata.id.trimmed();
  if (id.isEmpty() || metadata.displayName.trimmed().isEmpty()) {
    if (error) {
      *error = QStringLiteral("%1 的插件 ID 或显示名称为空").arg(source);
    }
    return false;
  }
  if (metadata.apiVersion != RouterPluginApiVersion) {
    if (error) {
      *error = QStringLiteral("插件 %1 的 API 版本为 %2，程序要求 %3")
                   .arg(id)
                   .arg(metadata.apiVersion)
                   .arg(RouterPluginApiVersion);
    }
    return false;
  }

  QWriteLocker locker(&lock_);
  if (entries_.contains(id)) {
    if (error) {
      *error = QStringLiteral("路由器插件 ID 重复：%1（%2）")
                   .arg(id, source);
    }
    return false;
  }
  auto normalized = metadata;
  normalized.id = id;
  entries_.insert(id, Entry{.metadata = std::move(normalized),
                            .factory = plugin,
                            .source = source});
  return true;
}

} // namespace bgptester
